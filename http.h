#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <stdexcept>
#include <cstdint>
#include <unordered_map>
#include <mutex>
#include <memory>

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

// --- WebSocket types ---

using WebSocketHandle = uint64_t;

struct WebSocketMessage {
    uint8_t opcode;  // 0x1 text, 0x2 binary
    std::string data;
};

struct WebSocketHandler {
    std::function<void(WebSocketHandle)> onOpen;
    std::function<void(WebSocketHandle, WebSocketMessage)> onMessage;
    std::function<void(WebSocketHandle)> onClose;
};

struct WebSocketFrame {
    bool fin = false;
    uint8_t opcode = 0;
    std::string payload;
};

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

// --- WebSocket functions (exposed for testing) ---

bool isWebSocketUpgrade(const ParsedRequest& request);
std::string buildWebSocketAcceptKey(const std::string& clientKey);
WebSocketFrame parseWebSocketFrame(const std::string& raw, size_t& bytesConsumed);
std::string buildWebSocketFrame(uint8_t opcode, const std::string& payload);
std::string sha1(const std::string& input);
std::string base64Encode(const std::string& input);

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

    void ws(const std::string& path, WebSocketHandler handler);
    bool send(WebSocketHandle handle, const std::string& message);
    bool send(WebSocketHandle handle, const std::vector<uint8_t>& message);
    void broadcast(const std::string& path, const std::string& message);
    void broadcast(const std::string& path, const std::vector<uint8_t>& message);
    void closeConnection(WebSocketHandle handle);
    std::vector<std::string> getRouteVariables(WebSocketHandle handle);

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

    struct WebSocketRoute {
        std::string pattern;
        WebSocketHandler handler;
    };

    struct WebSocketConnectionInfo {
        int socket;
        std::shared_ptr<std::mutex> writeMutex;
        std::vector<std::string> routeVariables;
        std::string routePattern;
        std::string fragmentBuffer;
        uint8_t fragmentOpcode = 0;
    };

    int port_;
    int serverSocket_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> activeConnectionCount_{0};
    std::thread acceptThread_;
    std::vector<Route> routes_;
    std::vector<StageHandler> stages_;
    std::vector<WebSocketRoute> wsRoutes_;
    std::unordered_map<WebSocketHandle, WebSocketConnectionInfo> wsConnections_;
    std::mutex wsConnectionsMutex_;
    std::atomic<WebSocketHandle> nextWsHandle_{1};

    void acceptLoop();
    void handleConnection(int clientSocket);
    void addRoute(const std::string& verb, const std::string& path, RouteHandler handler);
    RouteMatch findRoute(const std::string& verb, const std::string& path) const;
    NextFunction buildPipeline(RouteHandler matchedHandler, bool methodNotAllowed) const;
    std::vector<WebSocketHandle> findBroadcastTargets(const std::string& path);
    void handleWebSocketConnection(int clientSocket, const ParsedRequest& request, const WebSocketHandler& handler, const std::vector<std::string>& routeVariables, const std::string& routePattern);
    bool dispatchWebSocketFrame(const WebSocketFrame& frame, WebSocketHandle handle, const WebSocketHandler& handler, std::shared_ptr<std::mutex> writeMutex, int clientSocket);
};

} // namespace Http