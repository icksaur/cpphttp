#pragma once

#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include "src/platform/socket.h"

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

static std::ptrdiff_t send(Http::NativeSocket socket, const char* data,
                           size_t size, int) {
    const auto result = Http::Platform::writeComplete(
        socket, std::string_view(data, size),
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
    return result ? static_cast<std::ptrdiff_t>(result.bytesTransferred) : -1;
}

static std::ptrdiff_t recv(Http::NativeSocket socket, char* buffer,
                           size_t capacity, int) {
    const auto result = Http::Platform::receive(socket, buffer, capacity);
    return result.status == Http::Platform::SocketReadStatus::data
               ? static_cast<std::ptrdiff_t>(result.bytesTransferred)
               : -1;
}

static void close(Http::NativeSocket socket) {
    Http::Platform::closeSocket(socket);
}

// --- Helper: send raw HTTP request and get response ---

static std::string sendRequest(int port, const std::string& raw) {
    auto sock = Http::Platform::connectLoopback(port);
    if (sock == Http::invalidSocket) return "";

    send(sock, raw.c_str(), raw.size(), 0);

    std::string response;
    char buf[4096];
    while (true) {
        std::ptrdiff_t n = recv(sock, buf, sizeof(buf), 0);
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
    auto sp1 = raw.find(' ');
    if (sp1 == std::string::npos) return 0;
    auto sp2 = raw.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return 0;
    return std::stoi(raw.substr(sp1 + 1, sp2 - sp1 - 1));
}

static std::string getContentType(const std::string& raw) {
    auto pos = raw.find("Content-Type: ");
    if (pos == std::string::npos) return "";
    auto end = raw.find("\r\n", pos);
    return raw.substr(pos + 14, end - pos - 14);
}

static void waitForServer(int port, int maxRetries = 50) {
    for (int i = 0; i < maxRetries; i++) {
        auto sock = Http::Platform::connectLoopback(port);
        if (sock == Http::invalidSocket) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        close(sock);
        return;
    }
}
