#include "http.h"
#include <iostream>

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

    http.start();
    std::cout << "Server running on port 53000. Press Enter to stop." << std::endl;
    std::cin.get();
    http.stop();

    return 0;
}
