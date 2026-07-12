#include "socket.h"

#include <algorithm>
#include <climits>
#include <limits>
#include <winsock2.h>
#include <ws2tcpip.h>

namespace Http::Platform {
namespace {

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        available_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockRuntime() {
        if (available_) WSACleanup();
    }
    bool available() const { return available_; }

private:
    bool available_ = false;
};

bool ready() {
    static WinsockRuntime runtime;
    return runtime.available();
}

SOCKET native(NativeSocket socket) {
    return static_cast<SOCKET>(socket);
}

bool timeoutError(int error) {
    return error == WSAEWOULDBLOCK || error == WSAETIMEDOUT;
}

bool closedError(int error) {
    return error == WSAECONNRESET || error == WSAENOTCONN ||
           error == WSAESHUTDOWN || error == WSAECONNABORTED;
}

}

NativeSocket createTcpSocket() {
    if (!ready()) return invalidSocket;
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    return socket == INVALID_SOCKET ? invalidSocket
                                    : static_cast<NativeSocket>(socket);
}

NativeSocket acceptSocket(NativeSocket socket) {
    const SOCKET accepted = ::accept(native(socket), nullptr, nullptr);
    return accepted == INVALID_SOCKET ? invalidSocket
                                      : static_cast<NativeSocket>(accepted);
}

NativeSocket connectLoopback(int port) {
    auto socket = createTcpSocket();
    if (socket == invalidSocket) return invalidSocket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(native(socket), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == SOCKET_ERROR) {
        closeSocket(socket);
        return invalidSocket;
    }
    return socket;
}

bool setReuseAddress(NativeSocket socket) {
    const BOOL enabled = TRUE;
    return ::setsockopt(native(socket), SOL_SOCKET, SO_REUSEADDR,
                        reinterpret_cast<const char*>(&enabled),
                        sizeof(enabled)) == 0;
}

bool setReceiveTimeout(NativeSocket socket,
                       std::chrono::milliseconds timeout) {
    const DWORD value = static_cast<DWORD>(std::clamp<int64_t>(
        timeout.count(), 1, std::numeric_limits<DWORD>::max()));
    return ::setsockopt(native(socket), SOL_SOCKET, SO_RCVTIMEO,
                        reinterpret_cast<const char*>(&value),
                        sizeof(value)) == 0;
}

bool bindAny(NativeSocket socket, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    return ::bind(native(socket), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0;
}

bool listenSocket(NativeSocket socket, int backlog) {
    return ::listen(native(socket), backlog) == 0;
}

SocketReadResult receive(NativeSocket socket, char* buffer, size_t capacity) {
    const int requested =
        static_cast<int>(std::min(capacity, static_cast<size_t>(INT_MAX)));
    const int count = ::recv(native(socket), buffer, requested, 0);
    if (count > 0) {
        return {SocketReadStatus::data, static_cast<size_t>(count), 0};
    }
    if (count == 0) return {SocketReadStatus::closed, 0, 0};
    const int error = WSAGetLastError();
    if (timeoutError(error)) return {SocketReadStatus::timeout, 0, error};
    if (closedError(error)) return {SocketReadStatus::closed, 0, error};
    return {SocketReadStatus::error, 0, error};
}

SocketWriteResult writeComplete(NativeSocket socket, std::string_view data,
                                SocketWriteDeadline deadline) {
    if (socket == invalidSocket) {
        return SocketWriteResult::error(WSAENOTSOCK);
    }
    return completeWrite(
        data, deadline,
        [socket](std::string_view remaining, std::chrono::milliseconds timeout) {
            const DWORD value = static_cast<DWORD>(std::clamp<int64_t>(
                timeout.count(), 1, std::numeric_limits<DWORD>::max()));
            if (::setsockopt(native(socket), SOL_SOCKET, SO_SNDTIMEO,
                             reinterpret_cast<const char*>(&value),
                             sizeof(value)) != 0) {
                return SocketWriteResult::error(WSAGetLastError());
            }
            const int requested = static_cast<int>(
                std::min(remaining.size(), static_cast<size_t>(INT_MAX)));
            int count;
            do {
                count = ::send(native(socket), remaining.data(), requested, 0);
            } while (count == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
            if (count > 0) {
                return SocketWriteResult::complete(static_cast<size_t>(count));
            }
            if (count == 0) return SocketWriteResult::closed();
            const int error = WSAGetLastError();
            if (timeoutError(error)) return SocketWriteResult::timeout(0, error);
            if (closedError(error)) return SocketWriteResult::closed(error);
            return SocketWriteResult::error(error);
        });
}

void shutdownSocket(NativeSocket socket) {
    if (socket != invalidSocket) ::shutdown(native(socket), SD_BOTH);
}

void closeSocket(NativeSocket socket) {
    if (socket != invalidSocket) ::closesocket(native(socket));
}

}
