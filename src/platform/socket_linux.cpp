#include "socket.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace Http::Platform {
namespace {

int fd(NativeSocket socket) {
    return static_cast<int>(socket);
}

bool timeoutError(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == ETIMEDOUT;
}

bool closedError(int error) {
    return error == EPIPE || error == ECONNRESET || error == ENOTCONN ||
           error == ECONNABORTED;
}

}

NativeSocket createTcpSocket() {
    const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
    return socket < 0 ? invalidSocket : static_cast<NativeSocket>(socket);
}

NativeSocket acceptSocket(NativeSocket socket) {
    const int accepted = ::accept(fd(socket), nullptr, nullptr);
    return accepted < 0 ? invalidSocket : static_cast<NativeSocket>(accepted);
}

NativeSocket connectLoopback(int port) {
    auto socket = createTcpSocket();
    if (socket == invalidSocket) return invalidSocket;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::connect(fd(socket), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) < 0) {
        closeSocket(socket);
        return invalidSocket;
    }
    return socket;
}

bool setReuseAddress(NativeSocket socket) {
    int enabled = 1;
    return ::setsockopt(fd(socket), SOL_SOCKET, SO_REUSEADDR, &enabled,
                        sizeof(enabled)) == 0;
}

bool setReceiveTimeout(NativeSocket socket,
                       std::chrono::milliseconds timeout) {
    timeval value{};
    value.tv_sec = timeout.count() / 1000;
    value.tv_usec = (timeout.count() % 1000) * 1000;
    return ::setsockopt(fd(socket), SOL_SOCKET, SO_RCVTIMEO, &value,
                        sizeof(value)) == 0;
}

bool bindAny(NativeSocket socket, int port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<uint16_t>(port));
    return ::bind(fd(socket), reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0;
}

bool listenSocket(NativeSocket socket, int backlog) {
    return ::listen(fd(socket), backlog) == 0;
}

SocketReadResult receive(NativeSocket socket, char* buffer, size_t capacity) {
    const auto count = ::recv(fd(socket), buffer, capacity, 0);
    if (count > 0) {
        return {SocketReadStatus::data, static_cast<size_t>(count), 0};
    }
    if (count == 0) return {SocketReadStatus::closed, 0, 0};
    const int error = errno;
    if (timeoutError(error)) return {SocketReadStatus::timeout, 0, error};
    if (closedError(error)) return {SocketReadStatus::closed, 0, error};
    return {SocketReadStatus::error, 0, error};
}

SocketWriteResult writeComplete(NativeSocket socket, std::string_view data,
                                SocketWriteDeadline deadline) {
    if (socket == invalidSocket) return SocketWriteResult::error(EBADF);
    return completeWrite(
        data, deadline,
        [socket](std::string_view remaining, std::chrono::milliseconds timeout) {
            timeval value{};
            value.tv_sec = timeout.count() / 1000;
            value.tv_usec = (timeout.count() % 1000) * 1000;
            if (::setsockopt(fd(socket), SOL_SOCKET, SO_SNDTIMEO, &value,
                             sizeof(value)) != 0) {
                return SocketWriteResult::error(errno);
            }
            const size_t requested =
                std::min(remaining.size(), static_cast<size_t>(SSIZE_MAX));
            ssize_t count;
            do {
                count = ::send(fd(socket), remaining.data(), requested,
                               MSG_NOSIGNAL);
            } while (count < 0 && errno == EINTR);
            if (count > 0) {
                return SocketWriteResult::complete(static_cast<size_t>(count));
            }
            if (count == 0) return SocketWriteResult::closed();
            const int error = errno;
            if (timeoutError(error)) return SocketWriteResult::timeout(0, error);
            if (closedError(error)) return SocketWriteResult::closed(error);
            return SocketWriteResult::error(error);
        });
}

void shutdownSocket(NativeSocket socket) {
    if (socket != invalidSocket) ::shutdown(fd(socket), SHUT_RDWR);
}

void closeSocket(NativeSocket socket) {
    if (socket != invalidSocket) ::close(fd(socket));
}

}
