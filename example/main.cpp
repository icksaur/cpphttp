#include "http.h"
#include <iostream>
#include <vector>
#include <mutex>

int main() {
    Http::Server http(53000);

    http.get("/", [](Http::Context& ctx) {
        return Http::Ok("Hello, world!");
    });

    http.get("/:slug", [](Http::Context& ctx) {
        return Http::Ok(std::string("You asked for: ") + ctx.routeVariables[0]);
    });

    http.get("/params", [](Http::Context& ctx) {
        std::string response;
        for (const auto& kv : ctx.queryParameters) {
            response += kv.key + "=" + kv.value + "\n";
        }
        return Http::Ok(response);
    });

    http.post("/images", [](Http::Context& ctx) {
        return Http::Ok("Received " + std::to_string(ctx.requestBody.size()) + " bytes");
    });

    http.put("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Updated item " + ctx.routeVariables[0]);
    });

    http.patch("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Patched item " + ctx.routeVariables[0]);
    });

    http.del("/items/:id", [](Http::Context& ctx) {
        return Http::Ok("Deleted item " + ctx.routeVariables[0]);
    });

    // Logging middleware
    http.addStage([](Http::Context& ctx, Http::NextFunction next) {
        try {
            next(ctx);
            std::cout << ctx.verb << " " << ctx.route << " " << ctx.response.statusCode << std::endl;
        } catch (Http::HttpException& ex) {
            ctx.response = {ex.statusCode, ex.what(), "text/plain"};
            std::cout << ctx.verb << " " << ctx.route << " " << ctx.response.statusCode << " ERROR" << std::endl;
        }
    });

    // WebSocket echo endpoint
    http.ws("/echo", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            http.send(handle, "connected");
            std::cout << "WebSocket client connected to /echo" << std::endl;
        },
        .onMessage = [&](Http::WebSocketHandle handle, Http::WebSocketMessage msg) {
            http.send(handle, msg.data);
            std::cout << "Echo: " << msg.data << std::endl;
        },
        .onClose = [](Http::WebSocketHandle) {
            std::cout << "WebSocket client disconnected from /echo" << std::endl;
        }
    });

    // WebSocket server push (broadcast to all connected clients)
    std::vector<Http::WebSocketHandle> subscribers;
    std::mutex subMutex;

    http.ws("/notifications", {
        .onOpen = [&](Http::WebSocketHandle handle) {
            std::lock_guard lock(subMutex);
            subscribers.push_back(handle);
            std::cout << "Client subscribed to notifications (total: " << subscribers.size() << ")" << std::endl;
        },
        .onMessage = [](Http::WebSocketHandle, Http::WebSocketMessage) {},
        .onClose = [&](Http::WebSocketHandle handle) {
            std::lock_guard lock(subMutex);
            std::erase(subscribers, handle);
            std::cout << "Client unsubscribed from notifications (total: " << subscribers.size() << ")" << std::endl;
        }
    });

    // Trigger broadcast via HTTP POST
    http.post("/notify", [&](Http::Context& ctx) {
        std::lock_guard lock(subMutex);
        int sent = 0;
        for (auto handle : subscribers) {
            if (http.send(handle, ctx.requestBody)) {
                sent++;
            }
        }
        return Http::Ok("Sent to " + std::to_string(sent) + " clients");
    });

    http.start();
    std::cout << "Server running on port 53000" << std::endl;
    std::cout << "HTTP endpoints:" << std::endl;
    std::cout << "  GET  /" << std::endl;
    std::cout << "  GET  /params" << std::endl;
    std::cout << "  POST /images" << std::endl;
    std::cout << "  POST /notify" << std::endl;
    std::cout << "WebSocket endpoints:" << std::endl;
    std::cout << "  ws://localhost:53000/echo" << std::endl;
    std::cout << "  ws://localhost:53000/notifications" << std::endl;
    std::cout << "\nPress Enter to stop." << std::endl;
    std::cin.get();
    http.stop();

    return 0;
}
