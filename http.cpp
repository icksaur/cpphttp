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

// --- RAII socket closer ---

struct SocketGuard {
    int fd;
    explicit SocketGuard(int socket) : fd(socket) {}
    ~SocketGuard() { if (fd >= 0) close(fd); }
    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;
};

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

// --- SHA-1 and Base64 ---

std::string sha1(const std::string& input) {
    auto rotl = [](uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); };

    std::string padded = input;
    uint64_t bitLen = input.size() * 8;
    padded += '\x80';
    while ((padded.size() % 64) != 56) padded += '\x00';
    for (int i = 7; i >= 0; --i) padded += static_cast<char>((bitLen >> (i * 8)) & 0xFF);

    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476, h4 = 0xC3D2E1F0;

    for (size_t chunk = 0; chunk < padded.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                   (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                   static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::string result;
    for (uint32_t h : {h0, h1, h2, h3, h4}) {
        for (int i = 3; i >= 0; --i) result += static_cast<char>((h >> (i * 8)) & 0xFF);
    }
    return result;
}

std::string base64Encode(const std::string& input) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, valb = -6;
    for (uint8_t c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            result += table[(val >> valb) & 0x3F];
            valb -= 6;
        }
    }
    if (valb > -6) result += table[((val << 8) >> (valb + 8)) & 0x3F];
    while (result.size() % 4) result += '=';
    return result;
}

// --- WebSocket functions ---

bool isWebSocketUpgrade(const ParsedRequest& request) {
    std::string upgrade, connection;
    for (const auto& h : request.headers) {
        std::string lowerName = h.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName == "upgrade") {
            upgrade = h.value;
            std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
        }
        if (lowerName == "connection") {
            connection = h.value;
            std::transform(connection.begin(), connection.end(), connection.begin(), ::tolower);
        }
    }
    return upgrade == "websocket" && connection.find("upgrade") != std::string::npos;
}

std::string buildWebSocketAcceptKey(const std::string& clientKey) {
    static const std::string magicGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    return base64Encode(sha1(clientKey + magicGuid));
}

WebSocketFrame parseWebSocketFrame(const std::string& raw, size_t& bytesConsumed) {
    WebSocketFrame frame;
    bytesConsumed = 0;
    if (raw.size() < 2) return frame;

    uint8_t byte0 = static_cast<uint8_t>(raw[0]);
    uint8_t byte1 = static_cast<uint8_t>(raw[1]);

    frame.fin = (byte0 & 0x80) != 0;
    frame.opcode = byte0 & 0x0F;

    bool masked = (byte1 & 0x80) != 0;
    uint64_t payloadLen = byte1 & 0x7F;
    size_t pos = 2;

    if (payloadLen == 126) {
        if (raw.size() < 4) return frame;
        payloadLen = (static_cast<uint8_t>(raw[2]) << 8) | static_cast<uint8_t>(raw[3]);
        pos = 4;
    } else if (payloadLen == 127) {
        if (raw.size() < 10) return frame;
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLen = (payloadLen << 8) | static_cast<uint8_t>(raw[2 + i]);
        }
        pos = 10;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (raw.size() < pos + 4) return frame;
        for (int i = 0; i < 4; ++i) mask[i] = static_cast<uint8_t>(raw[pos + i]);
        pos += 4;
    }

    if (raw.size() < pos + payloadLen) return frame;

    frame.payload.reserve(payloadLen);
    for (uint64_t i = 0; i < payloadLen; ++i) {
        uint8_t byte = static_cast<uint8_t>(raw[pos + i]);
        if (masked) byte ^= mask[i % 4];
        frame.payload += static_cast<char>(byte);
    }

    bytesConsumed = pos + payloadLen;
    return frame;
}

std::string buildWebSocketFrame(uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);  // FIN=1

    size_t len = payload.size();
    if (len < 126) {
        frame += static_cast<char>(len);
    } else if (len < 65536) {
        frame += static_cast<char>(126);
        frame += static_cast<char>((len >> 8) & 0xFF);
        frame += static_cast<char>(len & 0xFF);
    } else {
        frame += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) {
            frame += static_cast<char>((len >> (i * 8)) & 0xFF);
        }
    }

    frame += payload;
    return frame;
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

void Server::ws(const std::string& path, WebSocketHandler handler) {
    wsRoutes_.push_back({normalizePath(path), std::move(handler)});
}

bool Server::send(WebSocketHandle handle, const std::string& message) {
    int sock;
    std::shared_ptr<std::mutex> writeMutex;
    {
        std::lock_guard lock(wsConnectionsMutex_);
        auto it = wsConnections_.find(handle);
        if (it == wsConnections_.end()) return false;
        sock = it->second.socket;
        writeMutex = it->second.writeMutex;
    }

    std::lock_guard writeLock(*writeMutex);
    std::string frame = buildWebSocketFrame(0x1, message);  // text frame
    ssize_t sent = ::send(sock, frame.c_str(), frame.size(), 0);
    return sent == static_cast<ssize_t>(frame.size());
}

bool Server::send(WebSocketHandle handle, const std::vector<uint8_t>& message) {
    int sock;
    std::shared_ptr<std::mutex> writeMutex;
    {
        std::lock_guard lock(wsConnectionsMutex_);
        auto it = wsConnections_.find(handle);
        if (it == wsConnections_.end()) return false;
        sock = it->second.socket;
        writeMutex = it->second.writeMutex;
    }

    std::lock_guard writeLock(*writeMutex);
    std::string payload(message.begin(), message.end());
    std::string frame = buildWebSocketFrame(0x2, payload);  // binary frame
    ssize_t sent = ::send(sock, frame.c_str(), frame.size(), 0);
    return sent == static_cast<ssize_t>(frame.size());
}

void Server::closeConnection(WebSocketHandle handle) {
    int sock;
    std::shared_ptr<std::mutex> writeMutex;
    {
        std::lock_guard lock(wsConnectionsMutex_);
        auto it = wsConnections_.find(handle);
        if (it == wsConnections_.end()) return;
        sock = it->second.socket;
        writeMutex = it->second.writeMutex;
    }

    std::lock_guard writeLock(*writeMutex);
    std::string closeFrame = buildWebSocketFrame(0x8, "");  // close frame
    ::send(sock, closeFrame.c_str(), closeFrame.size(), 0);
}

std::vector<std::string> Server::getRouteVariables(WebSocketHandle handle) {
    std::lock_guard lock(wsConnectionsMutex_);
    auto it = wsConnections_.find(handle);
    if (it == wsConnections_.end()) return {};
    return it->second.routeVariables;
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

    // Close all WebSocket connections
    std::vector<std::pair<int, std::shared_ptr<std::mutex>>> connections;
    {
        std::lock_guard lock(wsConnectionsMutex_);
        for (auto& [handle, conn] : wsConnections_) {
            connections.push_back({conn.socket, conn.writeMutex});
        }
    }
    
    // Send close frames without holding registry mutex
    for (auto& [sock, writeMutex] : connections) {
        std::lock_guard writeLock(*writeMutex);
        std::string closeFrame = buildWebSocketFrame(0x8, "");
        ::send(sock, closeFrame.c_str(), closeFrame.size(), 0);
        shutdown(sock, SHUT_RDWR);
    }

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
    SocketGuard guard(clientSocket);
    
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string rawRequest = readFullRequest(clientSocket);
    if (rawRequest.empty()) {
        return;
    }

    auto parsed = parseRequest(rawRequest);

    // Check for WebSocket upgrade
    if (isWebSocketUpgrade(parsed)) {
        for (const auto& wsRoute : wsRoutes_) {
            std::vector<std::string> vars;
            if (matchRoute(wsRoute.pattern, parsed.path, vars)) {
                handleWebSocketConnection(clientSocket, parsed, wsRoute.handler, vars);
                return;
            }
        }
        // No matching WebSocket route, fall through to HTTP 404
    }

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
    ::send(clientSocket, response.c_str(), response.size(), 0);
}

void Server::handleWebSocketConnection(int clientSocket, const ParsedRequest& request, 
                                        const WebSocketHandler& handler,
                                        const std::vector<std::string>& routeVariables) {
    // Extract WebSocket key
    std::string clientKey;
    for (const auto& h : request.headers) {
        std::string lowerName = h.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName == "sec-websocket-key") {
            clientKey = h.value;
            break;
        }
    }

    if (clientKey.empty()) {
        return;
    }

    // Send handshake response
    std::string acceptKey = buildWebSocketAcceptKey(clientKey);
    std::string handshake = 
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + acceptKey + "\r\n\r\n";
    
    if (::send(clientSocket, handshake.c_str(), handshake.size(), 0) < 0) {
        return;
    }

    // Register connection
    WebSocketHandle handle = nextWsHandle_++;
    std::shared_ptr<std::mutex> writeMutex = std::make_shared<std::mutex>();
    {
        std::lock_guard lock(wsConnectionsMutex_);
        wsConnections_[handle] = {
            clientSocket,
            writeMutex,
            routeVariables,
            "",
            0
        };
    }

    // Call onOpen
    if (handler.onOpen) {
        try {
            handler.onOpen(handle);
        } catch (...) {}
    }

    // Message loop
    static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024;
    char buffer[4096];
    std::string recvBuffer;
    bool done = false;

    while (running_ && !done) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesRead <= 0) break;

        recvBuffer.append(buffer, bytesRead);

        while (recvBuffer.size() >= 2) {
            // Parse frame
            size_t bytesConsumed = 0;
            WebSocketFrame frame = parseWebSocketFrame(recvBuffer, bytesConsumed);
            if (bytesConsumed == 0) break;  // Need more data

            recvBuffer.erase(0, bytesConsumed);

            // Handle frame
            if (frame.opcode == 0x8) {  // Close
                std::lock_guard writeLock(*writeMutex);
                std::string closeFrame = buildWebSocketFrame(0x8, "");
                ::send(clientSocket, closeFrame.c_str(), closeFrame.size(), 0);
                done = true;
                break;
            } else if (frame.opcode == 0x9) {  // Ping
                std::lock_guard writeLock(*writeMutex);
                std::string pongFrame = buildWebSocketFrame(0xA, frame.payload);
                ::send(clientSocket, pongFrame.c_str(), pongFrame.size(), 0);
            } else if (frame.opcode == 0x1 || frame.opcode == 0x2) {  // Text or Binary
                if (frame.fin) {
                    // Complete message
                    if (handler.onMessage) {
                        try {
                            handler.onMessage(handle, {frame.opcode, frame.payload});
                        } catch (...) {}
                    }
                } else {
                    // Start of fragmented message
                    std::lock_guard lock(wsConnectionsMutex_);
                    auto it = wsConnections_.find(handle);
                    if (it != wsConnections_.end()) {
                        it->second.fragmentOpcode = frame.opcode;
                        it->second.fragmentBuffer = frame.payload;
                    }
                }
            } else if (frame.opcode == 0x0) {  // Continuation
                std::lock_guard lock(wsConnectionsMutex_);
                auto it = wsConnections_.find(handle);
                if (it != wsConnections_.end()) {
                    it->second.fragmentBuffer += frame.payload;
                    if (it->second.fragmentBuffer.size() > MAX_MESSAGE_SIZE) {
                        done = true;
                        break;
                    }
                    if (frame.fin) {
                        // Complete fragmented message
                        if (handler.onMessage) {
                            try {
                                handler.onMessage(handle, {it->second.fragmentOpcode, it->second.fragmentBuffer});
                            } catch (...) {}
                        }
                        it->second.fragmentBuffer.clear();
                        it->second.fragmentOpcode = 0;
                    }
                }
            }
        }
    }

    // Unregister connection
    {
        std::lock_guard lock(wsConnectionsMutex_);
        wsConnections_.erase(handle);
    }

    // Call onClose
    if (handler.onClose) {
        try {
            handler.onClose(handle);
        } catch (...) {}
    }
}

} // namespace Http