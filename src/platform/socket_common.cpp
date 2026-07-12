#include "socket.h"

namespace Http::Platform {

SocketWriteResult completeWrite(std::string_view data,
                                SocketWriteDeadline deadline,
                                const WriteAttempt& attempt) {
    size_t transferred = 0;
    while (transferred < data.size()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return SocketWriteResult::timeout(transferred);
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (remaining.count() == 0) {
            remaining = std::chrono::milliseconds(1);
        }

        auto result = attempt(data.substr(transferred), remaining);
        if (result.bytesTransferred > data.size() - transferred) {
            return SocketWriteResult::error(result.nativeError, transferred);
        }
        transferred += result.bytesTransferred;
        if (result.status != SocketWriteStatus::complete) {
            result.bytesTransferred = transferred;
            return result;
        }
        if (result.bytesTransferred == 0) {
            return SocketWriteResult::closed(0, transferred);
        }
    }
    return SocketWriteResult::complete(transferred);
}

}
