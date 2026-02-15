#include "http.h"

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

static int passed = 0;
static int failed = 0;

#define ASSERT(cond) do { \
    if (!(cond)) { \
        std::cerr << "  FAIL: " << #cond << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
        failed++; \
    } else { \
        passed++; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        std::cerr << "  FAIL: " << #a << " == " << #b << " (got '" << _a << "' vs '" << _b << "') at " << __FILE__ << ":" << __LINE__ << std::endl; \
        failed++; \
    } else { \
        passed++; \
    } \
} while(0)

#define TEST(name) static void name()
#define RUN(name) do { \
    std::cout << "  " << #name << std::endl; \
    name(); \
} while(0)

// --- Helper: send raw HTTP request and get response ---

static std::string sendRequest(int port, const std::string& raw) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    send(sock, raw.c_str(), raw.size(), 0);

    std::string response;
    char buf[4096];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, n);
    }

    close(sock);
    return response;
}

static std::string httpGet(int port, const std::string& path) {
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    return sendRequest(port, req);
}

static std::string httpVerb(int port, const std::string& verb, const std::string& path, const std::string& body = "") {
    std::string req = verb + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!body.empty()) {
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += body;
    return sendRequest(port, req);
}

static std::string getResponseBody(const std::string& raw) {
    auto pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return raw.substr(pos + 4);
}

static int getResponseCode(const std::string& raw) {
    // HTTP/1.1 200 OK
    auto sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return 0;
    auto sp2 = raw.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return 0;
    return std::stoi(raw.substr(sp1 + 1, sp2 - sp1 - 1));
}

static void waitForServer(int port, int maxRetries = 50) {
    for (int i = 0; i < maxRetries; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            close(sock);
            return;
        }
        close(sock);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ============================================================
// Unit tests: parsing functions
// ============================================================

TEST(test_normalizePath) {
    ASSERT_EQ(Http::normalizePath("/hello"), std::string("/hello"));
    ASSERT_EQ(Http::normalizePath("hello"), std::string("/hello"));
    ASSERT_EQ(Http::normalizePath(""), std::string("/"));
    ASSERT_EQ(Http::normalizePath("/"), std::string("/"));
}

TEST(test_parseQueryString_empty) {
    auto params = Http::parseQueryString("");
    ASSERT_EQ((int)params.size(), 0);
}

TEST(test_parseQueryString_single) {
    auto params = Http::parseQueryString("key=value");
    ASSERT_EQ((int)params.size(), 1);
    ASSERT_EQ(params[0].key, std::string("key"));
    ASSERT_EQ(params[0].value, std::string("value"));
}

TEST(test_parseQueryString_multiple) {
    auto params = Http::parseQueryString("a=1&b=2&c=3");
    ASSERT_EQ((int)params.size(), 3);
    ASSERT_EQ(params[0].key, std::string("a"));
    ASSERT_EQ(params[1].key, std::string("b"));
    ASSERT_EQ(params[2].value, std::string("3"));
}

TEST(test_parseQueryString_no_value) {
    auto params = Http::parseQueryString("flag");
    ASSERT_EQ((int)params.size(), 1);
    ASSERT_EQ(params[0].key, std::string("flag"));
    ASSERT_EQ(params[0].value, std::string(""));
}

TEST(test_matchRoute_root) {
    std::vector<std::string> vars;
    ASSERT(Http::matchRoute("/", "/", vars));
    ASSERT_EQ((int)vars.size(), 0);
}

TEST(test_matchRoute_exact) {
    std::vector<std::string> vars;
    ASSERT(Http::matchRoute("/hello", "/hello", vars));
    ASSERT_EQ((int)vars.size(), 0);
}

TEST(test_matchRoute_variable) {
    std::vector<std::string> vars;
    ASSERT(Http::matchRoute("/:slug", "/hello", vars));
    ASSERT_EQ((int)vars.size(), 1);
    ASSERT_EQ(vars[0], std::string("hello"));
}

TEST(test_matchRoute_multi_variable) {
    std::vector<std::string> vars;
    ASSERT(Http::matchRoute("/users/:id/posts/:postId", "/users/42/posts/7", vars));
    ASSERT_EQ((int)vars.size(), 2);
    ASSERT_EQ(vars[0], std::string("42"));
    ASSERT_EQ(vars[1], std::string("7"));
}

TEST(test_matchRoute_no_match) {
    std::vector<std::string> vars;
    ASSERT(!Http::matchRoute("/hello", "/world", vars));
}

TEST(test_matchRoute_segment_count_mismatch) {
    std::vector<std::string> vars;
    ASSERT(!Http::matchRoute("/a/b", "/a", vars));
    ASSERT(!Http::matchRoute("/a", "/a/b", vars));
}

TEST(test_matchRoute_no_leading_slash) {
    std::vector<std::string> vars;
    ASSERT(Http::matchRoute("hello", "/hello", vars));
    ASSERT(Http::matchRoute("/hello", "hello", vars));
}

TEST(test_parseRequest_get) {
    std::string raw = "GET /path HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto req = Http::parseRequest(raw);
    ASSERT_EQ(req.verb, std::string("GET"));
    ASSERT_EQ(req.path, std::string("/path"));
    ASSERT_EQ(req.queryString, std::string(""));
    ASSERT_EQ(req.body, std::string(""));
}

TEST(test_parseRequest_post_with_body) {
    std::string raw = "POST /data HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nhello";
    auto req = Http::parseRequest(raw);
    ASSERT_EQ(req.verb, std::string("POST"));
    ASSERT_EQ(req.path, std::string("/data"));
    ASSERT_EQ(req.body, std::string("hello"));
}

TEST(test_parseRequest_query_string) {
    std::string raw = "GET /search?q=test&page=1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
    auto req = Http::parseRequest(raw);
    ASSERT_EQ(req.path, std::string("/search"));
    ASSERT_EQ(req.queryString, std::string("q=test&page=1"));
}

TEST(test_parseRequest_headers) {
    std::string raw = "GET / HTTP/1.1\r\nHost: localhost\r\nAccept: text/html\r\n\r\n";
    auto req = Http::parseRequest(raw);
    ASSERT_EQ((int)req.headers.size(), 2);
    ASSERT_EQ(req.headers[0].name, std::string("Host"));
    ASSERT_EQ(req.headers[0].value, std::string("localhost"));
    ASSERT_EQ(req.headers[1].name, std::string("Accept"));
    ASSERT_EQ(req.headers[1].value, std::string("text/html"));
}

TEST(test_formatResponse_200) {
    auto resp = Http::formatResponse(200, "OK body", "text/plain");
    ASSERT(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    ASSERT(resp.find("Content-Length: 7") != std::string::npos);
    ASSERT(resp.find("OK body") != std::string::npos);
}

TEST(test_formatResponse_404) {
    auto resp = Http::formatResponse(404, "nope", "text/plain");
    ASSERT(resp.find("HTTP/1.1 404 Not Found") != std::string::npos);
    ASSERT(resp.find("nope") != std::string::npos);
}

// ============================================================
// Integration tests
// ============================================================

static const int TEST_PORT = 19876;

static void setupTestServer(Http::Server& server) {
    server.get("/", [](Http::Context& ctx) {
        return Http::Ok("root");
    });

    server.get("/hello", [](Http::Context& ctx) {
        return Http::Ok("hello");
    });

    server.get("/:slug", [](Http::Context& ctx) {
        return Http::Ok("slug:" + ctx.routeVariables[0]);
    });

    server.get("/items/:id/detail", [](Http::Context& ctx) {
        return Http::Ok("item:" + ctx.routeVariables[0]);
    });

    server.get("/qparams", [](Http::Context& ctx) {
        std::string result;
        for (const auto& kv : ctx.queryParameters) {
            if (!result.empty()) result += ",";
            result += kv.key + "=" + kv.value;
        }
        return Http::Ok(result);
    });

    server.post("/data", [](Http::Context& ctx) {
        return Http::Ok("posted:" + ctx.requestBody);
    });

    server.put("/data", [](Http::Context& ctx) {
        return Http::Ok("put:" + ctx.requestBody);
    });

    server.patch("/data", [](Http::Context& ctx) {
        return Http::Ok("patched:" + ctx.requestBody);
    });

    server.del("/remove", [](Http::Context& ctx) {
        return Http::Ok("deleted");
    });

    server.get("/throw", [](Http::Context& ctx) -> Http::Response {
        throw Http::HttpException(403, "Forbidden");
    });

    server.get("/crash", [](Http::Context& ctx) -> Http::Response {
        throw std::runtime_error("oops");
    });

    server.get("/binary", [](Http::Context& ctx) {
        std::vector<uint8_t> data = {0x89, 0x50, 0x4E, 0x47};
        return Http::Ok(data, "application/octet-stream");
    });

    // Failure routes
    server.get("/bad-request", [](Http::Context& ctx) {
        return Http::Fail(400, "Invalid input");
    });

    server.get("/not-found", [](Http::Context& ctx) {
        return Http::Fail(404, "Resource not found");
    });

    server.get("/teapot", [](Http::Context& ctx) {
        return Http::Fail(418, "I'm a teapot");
    });

    server.get("/fail-no-body", [](Http::Context& ctx) {
        return Http::Fail(503);
    });

    server.post("/validate", [](Http::Context& ctx) {
        if (ctx.requestBody.empty()) {
            throw Http::HttpException(400, "Body required");
        }
        return Http::Ok("valid");
    });
}

TEST(test_integration_get_root) {
    auto resp = httpGet(TEST_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("root"));
}

TEST(test_integration_get_exact) {
    auto resp = httpGet(TEST_PORT, "/hello");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("hello"));
}

TEST(test_integration_route_variable) {
    auto resp = httpGet(TEST_PORT, "/foobar");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("slug:foobar"));
}

TEST(test_integration_multi_segment_variable) {
    auto resp = httpGet(TEST_PORT, "/items/99/detail");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("item:99"));
}

TEST(test_integration_query_params) {
    auto resp = httpGet(TEST_PORT, "/qparams?a=1&b=2");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("a=1,b=2"));
}

TEST(test_integration_auto_404) {
    // /nonexistent/path has 3 segments, no route matches
    auto resp = httpGet(TEST_PORT, "/no/match/here");
    ASSERT_EQ(getResponseCode(resp), 404);
}

TEST(test_integration_post) {
    auto resp = httpVerb(TEST_PORT, "POST", "/data", "payload");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("posted:payload"));
}

TEST(test_integration_put) {
    auto resp = httpVerb(TEST_PORT, "PUT", "/data", "updated");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("put:updated"));
}

TEST(test_integration_patch) {
    auto resp = httpVerb(TEST_PORT, "PATCH", "/data", "partial");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("patched:partial"));
}

TEST(test_integration_delete) {
    auto resp = httpVerb(TEST_PORT, "DELETE", "/remove");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("deleted"));
}

TEST(test_integration_wrong_verb_405) {
    auto resp = httpVerb(TEST_PORT, "POST", "/hello");
    ASSERT_EQ(getResponseCode(resp), 405);
}

TEST(test_integration_http_exception) {
    auto resp = httpGet(TEST_PORT, "/throw");
    ASSERT_EQ(getResponseCode(resp), 403);
}

TEST(test_integration_std_exception) {
    auto resp = httpGet(TEST_PORT, "/crash");
    ASSERT_EQ(getResponseCode(resp), 500);
}

TEST(test_integration_binary_response) {
    auto resp = httpGet(TEST_PORT, "/binary");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT(resp.find("application/octet-stream") != std::string::npos);
    auto body = getResponseBody(resp);
    ASSERT_EQ((int)body.size(), 4);
}

// --- Failure tests ---

TEST(test_fail_400) {
    auto resp = httpGet(TEST_PORT, "/bad-request");
    ASSERT_EQ(getResponseCode(resp), 400);
    ASSERT_EQ(getResponseBody(resp), std::string("Invalid input"));
}

TEST(test_fail_404_explicit) {
    auto resp = httpGet(TEST_PORT, "/not-found");
    ASSERT_EQ(getResponseCode(resp), 404);
    ASSERT_EQ(getResponseBody(resp), std::string("Resource not found"));
}

TEST(test_fail_arbitrary_code) {
    auto resp = httpGet(TEST_PORT, "/teapot");
    ASSERT_EQ(getResponseCode(resp), 418);
    ASSERT_EQ(getResponseBody(resp), std::string("I'm a teapot"));
}

TEST(test_fail_no_body) {
    auto resp = httpGet(TEST_PORT, "/fail-no-body");
    ASSERT_EQ(getResponseCode(resp), 503);
    ASSERT_EQ(getResponseBody(resp), std::string(""));
}

TEST(test_fail_via_exception) {
    // POST with empty body triggers HttpException(400)
    auto resp = httpVerb(TEST_PORT, "POST", "/validate");
    ASSERT_EQ(getResponseCode(resp), 400);
    ASSERT_EQ(getResponseBody(resp), std::string("Body required"));
}

TEST(test_fail_via_exception_success) {
    auto resp = httpVerb(TEST_PORT, "POST", "/validate", "data");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("valid"));
}

TEST(test_fail_helper_unit) {
    auto resp = Http::Fail(422, "Unprocessable", "application/json");
    ASSERT_EQ(resp.statusCode, 422);
    ASSERT_EQ(resp.body, std::string("Unprocessable"));
    ASSERT_EQ(resp.contentType, std::string("application/json"));
}

TEST(test_fail_helper_defaults) {
    auto resp = Http::Fail(500);
    ASSERT_EQ(resp.statusCode, 500);
    ASSERT_EQ(resp.body, std::string(""));
    ASSERT_EQ(resp.contentType, std::string("text/plain"));
}

// ============================================================
// Pipeline stage tests (separate server instance)
// ============================================================

static const int STAGE_PORT = 19877;

TEST(test_stage_wraps_handler) {
    Http::Server server(STAGE_PORT);
    bool stageRan = false;

    server.get("/", [](Http::Context& ctx) {
        return Http::Ok("ok");
    });

    server.addStage([&stageRan](Http::Context& ctx, Http::NextFunction next) {
        stageRan = true;
        next(ctx);
    });

    server.start();
    waitForServer(STAGE_PORT);

    auto resp = httpGet(STAGE_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("ok"));
    ASSERT(stageRan);

    server.stop();
}

TEST(test_stage_multiple_order) {
    Http::Server server(STAGE_PORT);
    std::string order;

    server.get("/", [](Http::Context& ctx) {
        return Http::Ok("ok");
    });

    server.addStage([&order](Http::Context& ctx, Http::NextFunction next) {
        order += "A";
        next(ctx);
        order += "a";
    });

    server.addStage([&order](Http::Context& ctx, Http::NextFunction next) {
        order += "B";
        next(ctx);
        order += "b";
    });

    server.start();
    waitForServer(STAGE_PORT);

    httpGet(STAGE_PORT, "/");

    // First-added stage is outermost: A enters, B enters, handler, B exits, A exits
    ASSERT_EQ(order, std::string("ABba"));

    server.stop();
}

TEST(test_stage_short_circuit) {
    Http::Server server(STAGE_PORT);
    bool handlerCalled = false;

    server.get("/", [&handlerCalled](Http::Context& ctx) {
        handlerCalled = true;
        return Http::Ok("should not reach");
    });

    server.addStage([](Http::Context& ctx, Http::NextFunction next) {
        // Do NOT call next - short-circuit
        ctx.response = {401, "Unauthorized", "text/plain"};
    });

    server.start();
    waitForServer(STAGE_PORT);

    auto resp = httpGet(STAGE_PORT, "/");

    ASSERT_EQ(getResponseCode(resp), 401);
    ASSERT_EQ(getResponseBody(resp), std::string("Unauthorized"));
    ASSERT(!handlerCalled);

    server.stop();
}

TEST(test_stage_catches_exception) {
    Http::Server server(STAGE_PORT);

    server.get("/", [](Http::Context& ctx) -> Http::Response {
        throw Http::HttpException(418, "I'm a teapot");
    });

    server.addStage([](Http::Context& ctx, Http::NextFunction next) {
        try {
            next(ctx);
        } catch (const Http::HttpException& ex) {
            ctx.response = {ex.statusCode, ex.what(), "text/plain"};
        }
    });

    server.start();
    waitForServer(STAGE_PORT);

    auto resp = httpGet(STAGE_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 418);
    ASSERT_EQ(getResponseBody(resp), std::string("I'm a teapot"));

    server.stop();
}

TEST(test_no_stages) {
    Http::Server server(STAGE_PORT);

    server.get("/", [](Http::Context& ctx) {
        return Http::Ok("direct");
    });

    server.start();
    waitForServer(STAGE_PORT);

    auto resp = httpGet(STAGE_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("direct"));

    server.stop();
}

// ============================================================
// WebSocket Unit Tests
// ============================================================

TEST(test_sha1_empty) {
    std::string hash = Http::sha1("");
    ASSERT_EQ((int)hash.size(), 20);
    // SHA-1 of empty string: da39a3ee5e6b4b0d3255bfef95601890afd80709
    ASSERT_EQ((uint8_t)hash[0], 0xda);
    ASSERT_EQ((uint8_t)hash[1], 0x39);
}

TEST(test_sha1_abc) {
    std::string hash = Http::sha1("abc");
    ASSERT_EQ((int)hash.size(), 20);
    // SHA-1 of "abc": a9993e364706816aba3e25717850c26c9cd0d89d
    ASSERT_EQ((uint8_t)hash[0], 0xa9);
    ASSERT_EQ((uint8_t)hash[1], 0x99);
}

TEST(test_base64_empty) {
    std::string encoded = Http::base64Encode("");
    ASSERT_EQ(encoded, std::string(""));
}

TEST(test_base64_basic) {
    ASSERT_EQ(Http::base64Encode("M"), std::string("TQ=="));
    ASSERT_EQ(Http::base64Encode("Ma"), std::string("TWE="));
    ASSERT_EQ(Http::base64Encode("Man"), std::string("TWFu"));
}

TEST(test_buildWebSocketAcceptKey) {
    std::string clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string acceptKey = Http::buildWebSocketAcceptKey(clientKey);
    ASSERT_EQ(acceptKey, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
}

TEST(test_isWebSocketUpgrade_positive) {
    Http::ParsedRequest req;
    req.headers = {
        {"Upgrade", "websocket"},
        {"Connection", "Upgrade"}
    };
    ASSERT(Http::isWebSocketUpgrade(req));
}

TEST(test_isWebSocketUpgrade_case_insensitive) {
    Http::ParsedRequest req;
    req.headers = {
        {"upgrade", "WebSocket"},
        {"connection", "upgrade"}
    };
    ASSERT(Http::isWebSocketUpgrade(req));
}

TEST(test_isWebSocketUpgrade_missing_upgrade) {
    Http::ParsedRequest req;
    req.headers = {
        {"Connection", "Upgrade"}
    };
    ASSERT(!Http::isWebSocketUpgrade(req));
}

TEST(test_isWebSocketUpgrade_wrong_values) {
    Http::ParsedRequest req;
    req.headers = {
        {"Upgrade", "HTTP/2.0"},
        {"Connection", "keep-alive"}
    };
    ASSERT(!Http::isWebSocketUpgrade(req));
}

TEST(test_buildWebSocketFrame_text) {
    std::string frame = Http::buildWebSocketFrame(0x1, "hello");
    ASSERT_EQ((int)frame.size(), 7);
    ASSERT_EQ((uint8_t)frame[0], 0x81);  // FIN=1, opcode=1
    ASSERT_EQ((uint8_t)frame[1], 5);     // length=5
    ASSERT_EQ(frame.substr(2), std::string("hello"));
}

TEST(test_buildWebSocketFrame_binary) {
    std::string payload = "data";
    std::string frame = Http::buildWebSocketFrame(0x2, payload);
    ASSERT_EQ((uint8_t)frame[0], 0x82);  // FIN=1, opcode=2
}

TEST(test_buildWebSocketFrame_close) {
    std::string frame = Http::buildWebSocketFrame(0x8, "");
    ASSERT_EQ((uint8_t)frame[0], 0x88);  // FIN=1, opcode=8
    ASSERT_EQ((uint8_t)frame[1], 0);     // length=0
}

TEST(test_buildWebSocketFrame_ping) {
    std::string frame = Http::buildWebSocketFrame(0x9, "ping");
    ASSERT_EQ((uint8_t)frame[0], 0x89);
}

TEST(test_buildWebSocketFrame_pong) {
    std::string frame = Http::buildWebSocketFrame(0xA, "pong");
    ASSERT_EQ((uint8_t)frame[0], 0x8A);
}

TEST(test_buildWebSocketFrame_126_length) {
    std::string payload(200, 'x');
    std::string frame = Http::buildWebSocketFrame(0x1, payload);
    ASSERT_EQ((uint8_t)frame[1], 126);
    ASSERT_EQ((uint8_t)frame[2], 0);
    ASSERT_EQ((uint8_t)frame[3], 200);
}

TEST(test_buildWebSocketFrame_64bit_length) {
    std::string payload(70000, 'x');
    std::string frame = Http::buildWebSocketFrame(0x1, payload);
    ASSERT_EQ((uint8_t)frame[1], 127);
}

TEST(test_parseWebSocketFrame_text_unmasked) {
    std::string raw = "\x81\x05hello";
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT(frame.fin);
    ASSERT_EQ((int)frame.opcode, 0x1);
    ASSERT_EQ(frame.payload, std::string("hello"));
    ASSERT_EQ((int)bytesConsumed, 7);
}

TEST(test_parseWebSocketFrame_text_masked) {
    // Masked "hello" with mask key [0x12, 0x34, 0x56, 0x78]
    std::string raw = "\x81\x85\x12\x34\x56\x78";
    raw += (char)(0x68 ^ 0x12);  // h
    raw += (char)(0x65 ^ 0x34);  // e
    raw += (char)(0x6c ^ 0x56);  // l
    raw += (char)(0x6c ^ 0x78);  // l
    raw += (char)(0x6f ^ 0x12);  // o
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT_EQ(frame.payload, std::string("hello"));
    ASSERT_EQ((int)bytesConsumed, 11);
}

TEST(test_parseWebSocketFrame_binary) {
    std::string raw;
    raw += '\x82';
    raw += '\x04';
    raw += "data";
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT(frame.fin);
    ASSERT_EQ((int)frame.opcode, 0x2);
    ASSERT_EQ(frame.payload, std::string("data"));
    ASSERT_EQ((int)bytesConsumed, 6);
}

TEST(test_parseWebSocketFrame_close) {
    std::string raw;
    raw += '\x88';
    raw += '\x00';
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT(frame.fin);
    ASSERT_EQ((int)frame.opcode, 0x8);
    ASSERT_EQ((int)bytesConsumed, 2);
}

TEST(test_parseWebSocketFrame_ping) {
    std::string raw;
    raw += '\x89';
    raw += '\x04';
    raw += "ping";
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT_EQ((int)frame.opcode, 0x9);
    ASSERT_EQ(frame.payload, std::string("ping"));
    ASSERT_EQ((int)bytesConsumed, 6);
}

TEST(test_parseWebSocketFrame_continuation_fin0) {
    std::string raw;
    raw += '\x00';
    raw += '\x03';
    raw += "abc";
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT(!frame.fin);
    ASSERT_EQ((int)frame.opcode, 0x0);
    ASSERT_EQ(frame.payload, std::string("abc"));
    ASSERT_EQ((int)bytesConsumed, 5);
}

TEST(test_parseWebSocketFrame_text_fin0) {
    std::string raw;
    raw += '\x01';
    raw += '\x03';
    raw += "abc";
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT(!frame.fin);
    ASSERT_EQ((int)frame.opcode, 0x1);
    ASSERT_EQ((int)bytesConsumed, 5);
}

TEST(test_parseWebSocketFrame_126_length) {
    std::string payload(200, 'x');
    std::string raw;
    raw += '\x81';
    raw += '\x7e';
    raw += '\x00';
    raw += '\xc8';
    raw += payload;
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT_EQ((int)frame.payload.size(), 200);
    ASSERT_EQ((int)bytesConsumed, 204);
}

// ============================================================
// WebSocket Integration Tests
// ============================================================

static std::string wsHandshake(int port, const std::string& path, const std::string& clientKey) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return "";
    }

    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + clientKey + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    std::string response;
    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n > 0) {
        response.append(buf, n);
    }

    close(sock);
    return response;
}

static int wsConnect(int port, const std::string& path) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    std::string clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string req = "GET " + path + " HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: " + clientKey + "\r\n"
                      "Sec-WebSocket-Version: 13\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    // Read handshake response
    char buf[4096];
    recv(sock, buf, sizeof(buf), 0);
    
    return sock;
}

static void wsSendTextFrame(int sock, const std::string& text) {
    std::string frame;
    frame += (char)0x81;  // FIN=1, opcode=1
    
    size_t len = text.size();
    if (len < 126) {
        frame += (char)(0x80 | len);  // MASK=1
    } else {
        frame += (char)0xFE;  // MASK=1, len=126
        frame += (char)((len >> 8) & 0xFF);
        frame += (char)(len & 0xFF);
    }
    
    // Mask key
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.append((char*)mask, 4);
    
    // Masked payload
    for (size_t i = 0; i < text.size(); ++i) {
        frame += (char)(text[i] ^ mask[i % 4]);
    }
    
    send(sock, frame.c_str(), frame.size(), 0);
}

static std::string wsRecvFrame(int sock) {
    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n <= 0) return "";
    
    std::string raw(buf, n);
    if (raw.size() < 2) return "";
    
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    return frame.payload;
}

TEST(test_ws_handshake) {
    constexpr int WS_PORT = 53100;
    Http::Server server(WS_PORT);
    
    bool opened = false;
    server.ws("/echo", {
        .onOpen = [&](Http::WebSocketHandle) { opened = true; },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    std::string clientKey = "dGhlIHNhbXBsZSBub25jZQ==";
    std::string response = wsHandshake(WS_PORT, "/echo", clientKey);
    
    ASSERT(response.find("101") != std::string::npos);
    ASSERT(response.find("Upgrade: websocket") != std::string::npos);
    ASSERT(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT(opened);
    
    server.stop();
}

TEST(test_ws_echo) {
    constexpr int WS_PORT = 53101;
    Http::Server server(WS_PORT);
    
    server.ws("/echo", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            server.send(handle, "connected");
        },
        .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
            server.send(handle, msg.data);
        },
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    int sock = wsConnect(WS_PORT, "/echo");
    ASSERT(sock >= 0);
    
    // Receive "connected" message
    std::string msg = wsRecvFrame(sock);
    ASSERT_EQ(msg, std::string("connected"));
    
    // Send echo test
    wsSendTextFrame(sock, "hello");
    msg = wsRecvFrame(sock);
    ASSERT_EQ(msg, std::string("hello"));
    
    close(sock);
    server.stop();
}

TEST(test_ws_server_push) {
    constexpr int WS_PORT = 53102;
    Http::Server server(WS_PORT);
    
    std::vector<Http::WebSocketHandle> clients;
    std::mutex clientsMutex;
    
    server.ws("/feed", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            std::lock_guard lock(clientsMutex);
            clients.push_back(handle);
        },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [&](Http::WebSocketHandle handle) {
            std::lock_guard lock(clientsMutex);
            std::erase(clients, handle);
        }
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    int sock1 = wsConnect(WS_PORT, "/feed");
    int sock2 = wsConnect(WS_PORT, "/feed");
    ASSERT(sock1 >= 0 && sock2 >= 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Push to all clients
    {
        std::lock_guard lock(clientsMutex);
        ASSERT_EQ((int)clients.size(), 2);
        for (auto handle : clients) {
            server.send(handle, "broadcast");
        }
    }
    
    std::string msg1 = wsRecvFrame(sock1);
    std::string msg2 = wsRecvFrame(sock2);
    ASSERT_EQ(msg1, std::string("broadcast"));
    ASSERT_EQ(msg2, std::string("broadcast"));
    
    close(sock1);
    close(sock2);
    server.stop();
}

TEST(test_ws_route_variables) {
    constexpr int WS_PORT = 53103;
    Http::Server server(WS_PORT);
    
    std::string capturedRoom;
    server.ws("/chat/:room", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            auto vars = server.getRouteVariables(handle);
            if (!vars.empty()) capturedRoom = vars[0];
        },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    int sock = wsConnect(WS_PORT, "/chat/lobby");
    ASSERT(sock >= 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_EQ(capturedRoom, std::string("lobby"));
    
    close(sock);
    server.stop();
}

TEST(test_ws_binary_message) {
    constexpr int WS_PORT = 53104;
    Http::Server server(WS_PORT);
    
    server.ws("/binary", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            std::vector<uint8_t> data = {0x01, 0x02, 0x03, 0x04};
            server.send(handle, data);
        },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    int sock = wsConnect(WS_PORT, "/binary");
    ASSERT(sock >= 0);
    
    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    ASSERT(n > 0);
    
    std::string raw(buf, n);
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT_EQ((int)frame.opcode, 0x2);  // binary
    ASSERT_EQ((int)frame.payload.size(), 4);
    
    close(sock);
    server.stop();
}

TEST(test_ws_invalid_handle) {
    constexpr int WS_PORT = 53105;
    Http::Server server(WS_PORT);
    
    server.ws("/test", {
        .onOpen = [](Http::WebSocketHandle) {},
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    bool result = server.send(99999, "test");
    ASSERT(!result);
    
    server.stop();
}

TEST(test_ws_nonexistent_route_404) {
    constexpr int WS_PORT = 53106;
    Http::Server server(WS_PORT);
    
    server.ws("/exists", {
        .onOpen = [](Http::WebSocketHandle) {},
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [](Http::WebSocketHandle) {}
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    std::string response = wsHandshake(WS_PORT, "/notfound", "dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT(response.find("404") != std::string::npos);
    
    server.stop();
}

TEST(test_ws_close_connection) {
    constexpr int WS_PORT = 53107;
    Http::Server server(WS_PORT);
    
    bool closed = false;
    server.ws("/test", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            server.closeConnection(handle);
        },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [&](Http::WebSocketHandle) {
            closed = true;
        }
    });
    
    server.start();
    waitForServer(WS_PORT);
    
    int sock = wsConnect(WS_PORT, "/test");
    ASSERT(sock >= 0);
    
    // Should receive close frame
    char buf[4096];
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    ASSERT(n > 0);
    
    std::string raw(buf, n);
    size_t bytesConsumed = 0;
    Http::WebSocketFrame frame = Http::parseWebSocketFrame(raw, bytesConsumed);
    ASSERT_EQ((int)frame.opcode, 0x8);  // close
    
    close(sock);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT(closed);
    
    server.stop();
}

// ============================================================

int main() {
    std::cout << "=== Unit tests ===" << std::endl;
    RUN(test_normalizePath);
    RUN(test_parseQueryString_empty);
    RUN(test_parseQueryString_single);
    RUN(test_parseQueryString_multiple);
    RUN(test_parseQueryString_no_value);
    RUN(test_matchRoute_root);
    RUN(test_matchRoute_exact);
    RUN(test_matchRoute_variable);
    RUN(test_matchRoute_multi_variable);
    RUN(test_matchRoute_no_match);
    RUN(test_matchRoute_segment_count_mismatch);
    RUN(test_matchRoute_no_leading_slash);
    RUN(test_parseRequest_get);
    RUN(test_parseRequest_post_with_body);
    RUN(test_parseRequest_query_string);
    RUN(test_parseRequest_headers);
    RUN(test_formatResponse_200);
    RUN(test_formatResponse_404);

    std::cout << "\n=== Integration tests ===" << std::endl;
    Http::Server server(TEST_PORT);
    setupTestServer(server);
    server.start();
    waitForServer(TEST_PORT);

    RUN(test_integration_get_root);
    RUN(test_integration_get_exact);
    RUN(test_integration_route_variable);
    RUN(test_integration_multi_segment_variable);
    RUN(test_integration_query_params);
    RUN(test_integration_auto_404);
    RUN(test_integration_post);
    RUN(test_integration_put);
    RUN(test_integration_patch);
    RUN(test_integration_delete);
    RUN(test_integration_wrong_verb_405);
    RUN(test_integration_http_exception);
    RUN(test_integration_std_exception);
    RUN(test_integration_binary_response);
    RUN(test_fail_400);
    RUN(test_fail_404_explicit);
    RUN(test_fail_arbitrary_code);
    RUN(test_fail_no_body);
    RUN(test_fail_via_exception);
    RUN(test_fail_via_exception_success);
    RUN(test_fail_helper_unit);
    RUN(test_fail_helper_defaults);

    server.stop();

    std::cout << "\n=== Pipeline stage tests ===" << std::endl;
    RUN(test_no_stages);
    RUN(test_stage_wraps_handler);
    RUN(test_stage_multiple_order);
    RUN(test_stage_short_circuit);
    RUN(test_stage_catches_exception);

    std::cout << "\n=== WebSocket unit tests ===" << std::endl;
    RUN(test_sha1_empty);
    RUN(test_sha1_abc);
    RUN(test_base64_empty);
    RUN(test_base64_basic);
    RUN(test_buildWebSocketAcceptKey);
    RUN(test_isWebSocketUpgrade_positive);
    RUN(test_isWebSocketUpgrade_case_insensitive);
    RUN(test_isWebSocketUpgrade_missing_upgrade);
    RUN(test_isWebSocketUpgrade_wrong_values);
    RUN(test_buildWebSocketFrame_text);
    RUN(test_buildWebSocketFrame_binary);
    RUN(test_buildWebSocketFrame_close);
    RUN(test_buildWebSocketFrame_ping);
    RUN(test_buildWebSocketFrame_pong);
    RUN(test_buildWebSocketFrame_126_length);
    RUN(test_buildWebSocketFrame_64bit_length);
    RUN(test_parseWebSocketFrame_text_unmasked);
    RUN(test_parseWebSocketFrame_text_masked);
    RUN(test_parseWebSocketFrame_binary);
    RUN(test_parseWebSocketFrame_close);
    RUN(test_parseWebSocketFrame_ping);
    RUN(test_parseWebSocketFrame_continuation_fin0);
    RUN(test_parseWebSocketFrame_text_fin0);
    RUN(test_parseWebSocketFrame_126_length);

    std::cout << "\n=== WebSocket integration tests ===" << std::endl;
    RUN(test_ws_handshake);
    RUN(test_ws_echo);
    RUN(test_ws_server_push);
    RUN(test_ws_route_variables);
    RUN(test_ws_binary_message);
    RUN(test_ws_invalid_handle);
    RUN(test_ws_nonexistent_route_404);
    RUN(test_ws_close_connection);

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    return failed > 0 ? 1 : 0;
}
