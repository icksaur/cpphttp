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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = httpGet(STAGE_PORT, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("ok"));

    // Give stage thread time to set the flag
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT(stageRan);

    server.stop();
}

static const int STAGE_PORT2 = 19878;

TEST(test_stage_multiple_order) {
    Http::Server server(STAGE_PORT2);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httpGet(STAGE_PORT2, "/");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // First-added stage is outermost: A enters, B enters, handler, B exits, A exits
    ASSERT_EQ(order, std::string("ABba"));

    server.stop();
}

static const int STAGE_PORT3 = 19879;

TEST(test_stage_short_circuit) {
    Http::Server server(STAGE_PORT3);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = httpGet(STAGE_PORT3, "/");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ASSERT_EQ(getResponseCode(resp), 401);
    ASSERT_EQ(getResponseBody(resp), std::string("Unauthorized"));
    ASSERT(!handlerCalled);

    server.stop();
}

static const int STAGE_PORT4 = 19880;

TEST(test_stage_catches_exception) {
    Http::Server server(STAGE_PORT4);

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
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = httpGet(STAGE_PORT4, "/");
    ASSERT_EQ(getResponseCode(resp), 418);
    ASSERT_EQ(getResponseBody(resp), std::string("I'm a teapot"));

    server.stop();
}

static const int STAGE_PORT5 = 19881;

TEST(test_no_stages) {
    Http::Server server(STAGE_PORT5);

    server.get("/", [](Http::Context& ctx) {
        return Http::Ok("direct");
    });

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto resp = httpGet(STAGE_PORT5, "/");
    ASSERT_EQ(getResponseCode(resp), 200);
    ASSERT_EQ(getResponseBody(resp), std::string("direct"));

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
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

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

    std::cout << "\n=== Results ===" << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;

    return failed > 0 ? 1 : 0;
}
