#pragma once

#include "http.h"

#include <chrono>
#include <functional>
#include <string_view>

namespace Http::Platform {

enum class SocketReadStatus {
    data,
    timeout,
    closed,
    error,
};

struct SocketReadResult {
    SocketReadStatus status = SocketReadStatus::error;
    size_t bytesTransferred = 0;
    int nativeError = 0;
};

using WriteAttempt = std::function<SocketWriteResult(
    std::string_view, std::chrono::milliseconds)>;

SocketWriteResult completeWrite(std::string_view data,
                                SocketWriteDeadline deadline,
                                const WriteAttempt& attempt);
SocketWriteResult writeComplete(NativeSocket socket, std::string_view data,
                                SocketWriteDeadline deadline);

NativeSocket createTcpSocket();
NativeSocket acceptSocket(NativeSocket socket);
NativeSocket connectLoopback(int port);
bool setReuseAddress(NativeSocket socket);
bool setReceiveTimeout(NativeSocket socket, std::chrono::milliseconds timeout);
bool bindAny(NativeSocket socket, int port);
bool listenSocket(NativeSocket socket, int backlog);
SocketReadResult receive(NativeSocket socket, char* buffer, size_t capacity);
void shutdownSocket(NativeSocket socket);
void closeSocket(NativeSocket socket);

}
