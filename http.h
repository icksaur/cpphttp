#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cstdint>

namespace Http {

// --- Data types ---

struct QueryParameterKeyValue {
    std::string key;
    std::string value;
};

struct Header {
    std::string name;
    std::string value;
};

struct Response {
    int statusCode;
    std::string body;
    std::string contentType;
};

struct Context {
    std::string verb;
    std::string route;
    std::vector<std::string> routeVariables;
    std::vector<QueryParameterKeyValue> queryParameters;
    std::vector<Header> requestHeaders;
    std::string requestBody;
    Response response = {0, "", "text/plain"};
};

struct HttpException : public std::runtime_error {
    int statusCode;
    HttpException(int code, const std::string& message);
};

// --- Function types ---

using RouteHandler = std::function<Response(Context&)>;
using NextFunction = std::function<void(Context&)>;
using StageHandler = std::function<void(Context&, NextFunction)>;

// --- Response helpers ---

Response Ok(const std::string& body, const std::string& contentType = "text/plain");
Response Ok(const std::vector<uint8_t>& body, const std::string& contentType);
Response Fail(int statusCode, const std::string& body = "", const std::string& contentType = "text/plain");

// --- Parsing (exposed for testing) ---

struct ParsedRequest {
    std::string verb;
    std::string path;
    std::string queryString;
    std::vector<Header> headers;
    std::string body;
};

ParsedRequest parseRequest(const std::string& raw);
std::vector<QueryParameterKeyValue> parseQueryString(const std::string& queryString);
bool matchRoute(const std::string& pattern, const std::string& path, std::vector<std::string>& outVariables);
std::string formatResponse(int statusCode, const std::string& body, const std::string& contentType);
std::string normalizePath(const std::string& path);

// --- Server ---

class Server {
public:
    explicit Server(int port);
    ~Server();

    void get(const std::string& path, RouteHandler handler);
    void post(const std::string& path, RouteHandler handler);
    void put(const std::string& path, RouteHandler handler);
    void patch(const std::string& path, RouteHandler handler);
    void del(const std::string& path, RouteHandler handler);
    void addStage(StageHandler handler);

    void start();
    void stop();

private:
    struct Route {
        std::string verb;
        std::string pattern;
        RouteHandler handler;
    };

    struct RouteMatch {
        RouteHandler handler;
        std::vector<std::string> variables;
        bool methodNotAllowed = false;
    };

    int port_;
    int serverSocket_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> activeConnectionCount_{0};
    std::thread acceptThread_;
    std::vector<Route> routes_;
    std::vector<StageHandler> stages_;

    void acceptLoop();
    void handleConnection(int clientSocket);
    void addRoute(const std::string& verb, const std::string& path, RouteHandler handler);
    RouteMatch findRoute(const std::string& verb, const std::string& path) const;
    NextFunction buildPipeline(RouteHandler matchedHandler, bool methodNotAllowed) const;
};

} // namespace Http