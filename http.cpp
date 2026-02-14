#include "http.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sys/time.h>
#include <algorithm>
#include <limits>
#include <format>

namespace Http {

// --- HttpException ---

HttpException::HttpException(int code, const std::string& message)
    : std::runtime_error(message), statusCode(code) {}

// --- Response helpers ---

Response Ok(const std::string& body, const std::string& contentType) {
    return {200, body, contentType};
}

Response Ok(const std::vector<uint8_t>& body, const std::string& contentType) {
    return {200, std::string(body.begin(), body.end()), contentType};
}

Response Fail(int statusCode, const std::string& body, const std::string& contentType) {
    return {statusCode, body, contentType};
}

// --- Parsing ---

std::string normalizePath(const std::string& path) {
    if (path.empty() || !path.starts_with('/')) {
        return "/" + path;
    }
    return path;
}

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> segments;
    auto norm = normalizePath(path);
    size_t pos = 1;
    while (pos < norm.size()) {
        auto next = norm.find('/', pos);
        if (next == std::string::npos) next = norm.size();
        if (next > pos) segments.emplace_back(norm, pos, next - pos);
        pos = next + 1;
    }
    return segments;
}

std::vector<QueryParameterKeyValue> parseQueryString(const std::string& queryString) {
    std::vector<QueryParameterKeyValue> params;
    if (queryString.empty()) return params;

    size_t pos = 0;
    while (pos < queryString.size()) {
        auto amp = queryString.find('&', pos);
        if (amp == std::string::npos) amp = queryString.size();
        if (amp > pos) {
            auto pair = queryString.substr(pos, amp - pos);
            auto eq = pair.find('=');
            if (eq != std::string::npos) {
                params.push_back({pair.substr(0, eq), pair.substr(eq + 1)});
            } else {
                params.push_back({pair, ""});
            }
        }
        pos = amp + 1;
    }
    return params;
}

bool matchRoute(const std::string& pattern, const std::string& path, std::vector<std::string>& outVariables) {
    outVariables.clear();

    auto patternSegments = splitPath(pattern);
    auto pathSegments = splitPath(path);

    if (patternSegments.empty() && pathSegments.empty()) {
        return true;
    }

    if (patternSegments.size() != pathSegments.size()) {
        return false;
    }

    for (size_t i = 0; i < patternSegments.size(); ++i) {
        if (patternSegments[i].starts_with(':')) {
            outVariables.push_back(pathSegments[i]);
        } else if (patternSegments[i] != pathSegments[i]) {
            return false;
        }
    }

    return true;
}

ParsedRequest parseRequest(const std::string& raw) {
    ParsedRequest req;

    auto lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return req;

    std::string requestLine = raw.substr(0, lineEnd);

    auto firstSpace = requestLine.find(' ');
    auto lastSpace = requestLine.rfind(' ');
    if (firstSpace == std::string::npos || firstSpace == lastSpace) return req;

    req.verb = requestLine.substr(0, firstSpace);
    std::string fullPath = requestLine.substr(firstSpace + 1, lastSpace - firstSpace - 1);

    auto qmark = fullPath.find('?');
    if (qmark != std::string::npos) {
        req.path = fullPath.substr(0, qmark);
        req.queryString = fullPath.substr(qmark + 1);
    } else {
        req.path = fullPath;
    }

    // Parse headers
    size_t pos = lineEnd + 2;
    while (pos < raw.size()) {
        auto nextLine = raw.find("\r\n", pos);
        if (nextLine == std::string::npos || nextLine == pos) break;

        std::string headerLine = raw.substr(pos, nextLine - pos);
        auto colon = headerLine.find(':');
        if (colon != std::string::npos) {
            std::string name = headerLine.substr(0, colon);
            std::string value = headerLine.substr(colon + 1);
            auto start = value.find_first_not_of(' ');
            if (start != std::string::npos) {
                value = value.substr(start);
            }
            req.headers.push_back({name, value});
        }
        pos = nextLine + 2;
    }

    // Body is everything after \r\n\r\n
    auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        req.body = raw.substr(headerEnd + 4);
    }

    return req;
}

std::string formatResponse(int statusCode, const std::string& body, const std::string& contentType) {
    const char* statusText;
    switch (statusCode) {
        case 200: statusText = "OK"; break;
        case 201: statusText = "Created"; break;
        case 204: statusText = "No Content"; break;
        case 400: statusText = "Bad Request"; break;
        case 403: statusText = "Forbidden"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 500: statusText = "Internal Server Error"; break;
        default:  statusText = "Unknown"; break;
    }

    return std::format(
        "HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
        statusCode, statusText, contentType, body.size(), body);
}

// --- Server ---

Server::Server(int port) : port_(port) {}

Server::~Server() {
    if (running_) {
        stop();
    }
}

void Server::get(const std::string& path, RouteHandler handler) {
    addRoute("GET", path, std::move(handler));
}

void Server::post(const std::string& path, RouteHandler handler) {
    addRoute("POST", path, std::move(handler));
}

void Server::put(const std::string& path, RouteHandler handler) {
    addRoute("PUT", path, std::move(handler));
}

void Server::patch(const std::string& path, RouteHandler handler) {
    addRoute("PATCH", path, std::move(handler));
}

void Server::del(const std::string& path, RouteHandler handler) {
    addRoute("DELETE", path, std::move(handler));
}

void Server::addStage(StageHandler handler) {
    stages_.push_back(std::move(handler));
}

void Server::addRoute(const std::string& verb, const std::string& path, RouteHandler handler) {
    routes_.push_back({verb, normalizePath(path), std::move(handler)});
}

void Server::start() {
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
        throw HttpException(500, "Failed to create socket");
    }

    int opt = 1;
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(serverSocket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(serverSocket_);
        serverSocket_ = -1;
        throw HttpException(500, "Failed to bind to port");
    }

    if (listen(serverSocket_, 16) < 0) {
        close(serverSocket_);
        serverSocket_ = -1;
        throw HttpException(500, "Failed to listen");
    }

    running_ = true;
    acceptThread_ = std::thread(&Server::acceptLoop, this);
}

void Server::stop() {
    running_ = false;
    if (serverSocket_ >= 0) {
        shutdown(serverSocket_, SHUT_RDWR);
        close(serverSocket_);
        serverSocket_ = -1;
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
    while (activeConnectionCount_ > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void Server::acceptLoop() {
    while (running_) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int clientSocket = accept(serverSocket_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);

        if (clientSocket < 0) {
            continue;
        }

        activeConnectionCount_++;
        std::thread([this, clientSocket]() {
            try {
                handleConnection(clientSocket);
            } catch (...) {}
            activeConnectionCount_--;
        }).detach();
    }
}

// --- Request reading ---

static constexpr size_t MAX_REQUEST_SIZE = 1024 * 1024;

static int extractContentLength(const std::string& rawHeaders) {
    std::string lower = rawHeaders;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    auto pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    auto valueStart = pos + 15;
    while (valueStart < lower.size() && lower[valueStart] == ' ') valueStart++;
    auto valueEnd = lower.find("\r\n", valueStart);
    if (valueEnd == std::string::npos) valueEnd = lower.size();
    try {
        return std::stoi(rawHeaders.substr(valueStart, valueEnd - valueStart));
    } catch (...) {
        return 0;
    }
}

static std::string readFullRequest(int clientSocket) {
    std::string raw;
    char buffer[4096];

    while (true) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) return "";
        raw.append(buffer, bytesRead);
        if (raw.size() > MAX_REQUEST_SIZE) return "";
        if (raw.find("\r\n\r\n") != std::string::npos) break;
    }

    auto headerEnd = raw.find("\r\n\r\n");
    int contentLength = extractContentLength(raw.substr(0, headerEnd));

    size_t bodyReceived = raw.size() - (headerEnd + 4);
    while (static_cast<int>(bodyReceived) < contentLength) {
        if (raw.size() > MAX_REQUEST_SIZE) return "";
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) break;
        raw.append(buffer, bytesRead);
        bodyReceived += bytesRead;
    }

    return raw;
}

// --- Routing and pipeline ---

Server::RouteMatch Server::findRoute(const std::string& verb, const std::string& path) const {
    RouteMatch best;
    int bestVarCount = std::numeric_limits<int>::max();
    bool pathExists = false;

    for (const auto& route : routes_) {
        std::vector<std::string> vars;
        if (matchRoute(route.pattern, path, vars)) {
            pathExists = true;
            if (route.verb == verb && static_cast<int>(vars.size()) < bestVarCount) {
                bestVarCount = static_cast<int>(vars.size());
                best.handler = route.handler;
                best.variables = vars;
            }
        }
    }

    if (!best.handler && pathExists) {
        best.methodNotAllowed = true;
    }

    return best;
}

NextFunction Server::buildPipeline(RouteHandler matchedHandler, bool methodNotAllowed) const {
    NextFunction innermost;

    if (matchedHandler) {
        innermost = [handler = std::move(matchedHandler)](Context& c) {
            c.response = handler(c);
        };
    } else if (methodNotAllowed) {
        innermost = [](Context& c) {
            c.response = {405, "Method Not Allowed", "text/plain"};
        };
    } else {
        innermost = [](Context& c) {
            c.response = {404, "Not Found", "text/plain"};
        };
    }

    NextFunction pipeline = innermost;
    for (int i = static_cast<int>(stages_.size()) - 1; i >= 0; --i) {
        auto stage = stages_[i];
        auto next = pipeline;
        pipeline = [stage, next](Context& c) {
            stage(c, next);
        };
    }

    return pipeline;
}

void Server::handleConnection(int clientSocket) {
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string rawRequest = readFullRequest(clientSocket);
    if (rawRequest.empty()) {
        close(clientSocket);
        return;
    }

    auto parsed = parseRequest(rawRequest);

    Context ctx;
    ctx.verb = parsed.verb;
    ctx.route = parsed.path;
    ctx.queryParameters = parseQueryString(parsed.queryString);
    ctx.requestHeaders = parsed.headers;
    ctx.requestBody = parsed.body;

    auto match = findRoute(parsed.verb, parsed.path);
    ctx.routeVariables = match.variables;

    auto pipeline = buildPipeline(match.handler, match.methodNotAllowed);

    try {
        pipeline(ctx);
    } catch (const HttpException& ex) {
        ctx.response = {ex.statusCode, ex.what(), "text/plain"};
    } catch (const std::exception&) {
        ctx.response = {500, "Internal Server Error", "text/plain"};
    }

    std::string response = formatResponse(ctx.response.statusCode, ctx.response.body, ctx.response.contentType);
    send(clientSocket, response.c_str(), response.size(), 0);
    close(clientSocket);
}

} // namespace Http